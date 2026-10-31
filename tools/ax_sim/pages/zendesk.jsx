import React, { useState } from 'react';
import { createRoot } from 'react-dom/client';
import { ThemeProvider } from '@zendeskgarden/react-theming';
import { Dropdown, Trigger, Menu, Item } from '@zendeskgarden/react-dropdowns';
import { Modal, Header, Body, Footer, FooterItem, Close } from '@zendeskgarden/react-modals';
import { Button } from '@zendeskgarden/react-buttons';

const q = new URLSearchParams(location.search);
const portal = q.get('portal') === '1';

function Tickets() {
  const rows = [];
  for (let i = 0; i < 18; i++)
    rows.push(<tr key={i} style={{ height: 40 }}>
      <td><input type="checkbox" aria-label={'select ' + i} /></td>
      <td><a href={'#t' + i}>Ticket #{1000 + i}: printer on floor {i} is out of toner again</a></td>
      <td><a href={'#u' + i}>Requester {i}</a></td>
      <td>Open</td><td><a href={'#g' + i}>Support</a></td></tr>);
  return <table style={{ width: '100%', borderCollapse: 'collapse' }}><tbody>{rows}</tbody></table>;
}

function App() {
  const [modal, setModal] = useState(false);
  const menuProps = portal ? { appendToNode: document.body } : {};
  return <ThemeProvider>
    <div style={{ display: 'flex', gap: 16, padding: 12, borderBottom: '1px solid #ddd', alignItems: 'center' }}>
      <b>Zendesk mock</b>
      <Dropdown onSelect={v => { if (v === 'macro') setModal(true); }}>
        <Trigger><Button id="add">+ Add</Button></Trigger>
        <Menu placement="bottom-start" {...menuProps}>
          <Item value="ticket">New ticket</Item>
          <Item value="user">New user</Item>
          <Item value="org">New organization</Item>
          <Item value="macro">Apply macro</Item>
          <Item value="search">Search tickets</Item>
        </Menu>
      </Dropdown>
      <Dropdown>
        <Trigger><Button id="views">Views</Button></Trigger>
        <Menu placement="bottom-start" {...menuProps}>
          <Item value="a">Your unsolved tickets</Item>
          <Item value="b">Unassigned tickets</Item>
          <Item value="c">All unsolved tickets</Item>
          <Item value="d">Recently updated tickets</Item>
          <Item value="e">Pending tickets</Item>
          <Item value="f">Recently solved tickets</Item>
        </Menu>
      </Dropdown>
      <Button id="openmodal" onClick={() => setModal(true)}>Merge</Button>
    </div>
    <Tickets />
    {modal && <Modal onClose={() => setModal(false)}>
      <Header>Merge tickets</Header>
      <Body><p>Merge ticket #1001 into #1002?</p><a href="#more">Learn more about merging</a></Body>
      <Footer><FooterItem><Button onClick={() => setModal(false)} isBasic>Cancel</Button></FooterItem>
        <FooterItem><Button isPrimary onClick={() => setModal(false)}>Confirm and merge</Button></FooterItem></Footer>
      <Close aria-label="Close modal" />
    </Modal>}
  </ThemeProvider>;
}
createRoot(document.getElementById('root')).render(<App />);
